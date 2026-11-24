# std/regex reference

This package implements a small UTF-8 regular-expression engine. Patterns are
compiled to a Thompson NFA and simulated with a state set; ordinary matching is
not a backtracking matcher. All positions and lengths are Unicode code point
indices, not bytes. `Match` uses a half-open range: `start` is inclusive and
`end` is exclusive.

The top-level standard-library summary lists the original API. This file is the
detailed reference for the extended API and its limits.

## Public API

```wz
pub enum Error {
    Syntax(offset usize);
    PatternTooLarge(offset usize);
    NestingTooDeep(offset usize);
}

pub enum CaptureError {
    Unavailable;
}

pub struct Match {
    pub start usize;
    pub end usize;
}

pub struct Captures {
    pub start usize;
    pub end usize;
    // source string and group vector are private
}

pub struct Pattern { /* opaque */ }

pub func compile(pattern string) Pattern throws Error
pub func compile_captures(pattern string) Pattern throws Error

pub func find(compiled Pattern, value string) option.Option[Match]
pub func find_from(compiled Pattern, value string, from usize) option.Option[Match]
pub func find_all(compiled Pattern, value string) Vec[Match]
pub func is_match(compiled Pattern, value string) bool

pub func split(compiled Pattern, value string) Vec[string]
pub func split_n(compiled Pattern, value string, limit usize) Vec[string]

pub func replace_all(compiled Pattern, value string, replacement string) string
pub func replace_n(compiled Pattern, value string, replacement string,
    limit usize) string

pub func find_captures(compiled Pattern,
    value string) option.Option[Captures] throws CaptureError
pub func find_captures_from(compiled Pattern, value string,
    from usize) option.Option[Captures] throws CaptureError
pub func group_count(captures Captures) usize
pub func group(captures Captures, index usize) option.Option[Match]
pub func group_text(captures Captures, index usize) option.Option[string]
```

`compile` emits capture tags automatically when the pattern has at most 32
capture groups, so `find_captures(compile("([a-z]+)=([0-9]+)"), text)` works
without a separate compile call. The same `Pattern` can be used with `find`,
`find_from`, `find_all`, `is_match`, `split`, `replace_all`, and `replace_n`;
those ordinary operations treat tag states as epsilon and do not copy tags.
`find_captures` runs the tagged thread path. A pattern with no parentheses has
capture metadata with zero groups, so `group_count` is 0 and
`group_text(captures, 0)` is the overall match text.

`compile` preserves the previous compile behavior for patterns that cannot use
the capture budget. If a pattern needs more than 32 groups, or adding tag
states would exceed the existing NFA state limit, `compile` retries with
grouping-only states. Such a `Pattern` still matches exactly as before and is
accepted by every ordinary API, but it has no capture metadata. Calling
`find_captures` or `find_captures_from` on it throws
`CaptureError.Unavailable`; it never silently returns zero or truncated
groups. `CaptureError` is a separate public enum, so the published compile
`Error` cases and existing exhaustive switches remain unchanged.

`compile_captures` is the explicitly checked form. It never falls back: a
pattern that exceeds the 32-group or tag-state budget throws
`Error.PatternTooLarge` from the compile call. Use it when capture availability
must be known before searching.

## Search semantics

`find` returns the earliest match start and, for that start, the longest match
end. This is the existing leftmost-longest behavior.

`find_from(compiled, value, from)` searches the suffix beginning at code point
index `from`:

- A match may begin at `from` even if a longer match beginning earlier
  overlaps `from`. For example, `a+` on `"baaacaa"` from index 2 returns
  `[2, 4)`, not the whole first run `[1, 4)` and not the later run `[5, 7)`.
- A match beginning before `from` is never returned.
- `from > text.len(value)` returns `None`.
- `from == text.len(value)` can still return an empty match at the end.
- `^` means "position zero of the whole input", never "position `from`".
  Therefore `find_from(compile("^"), "abc", 1)` is `None`, while
  `find_from(compile("$"), "abc", 2)` returns `[3, 3)`.

`find_all` returns non-overlapping matches in order using the same zero-width
advance rule as split and replace: after an empty match at `p` the next search
begins at `p + 1`; after a non-empty match it begins at the match end. An empty
match at the whole-input end is included once and then stops. For example,
`find_all(compile(""), "ab")` is `[[0,0], [1,1], [2,2]]` and
`find_all(compile("a*"), "ba")` is `[[0,0], [1,2], [2,2]]`. A tagged Pattern
uses the same ordinary matching path and ignores its tag states.

`split` and `split_n` use non-overlapping matches. `split_n` follows
`std/text.split_n`: limit 0 returns an empty Vec, limit 1 returns `[value]`,
and otherwise at most `limit` pieces are returned with the last piece holding
the remaining input. A trailing separator produces a trailing empty piece.

An empty match is a real separator at its position. The piece before it and
the piece after the last separator are both kept, including empty pieces.
After an empty match at position `p`, the next search begins at `p + 1`, so a
zero-width pattern cannot loop. The character at `p` is copied into the next
piece. Examples:

```wz
split(compile(""), "ab")      // ["", "a", "b", ""]
split(compile(""), "")        // ["", ""]
split(compile("a*"), "ba")    // ["", "b", "", ""]
split(compile("^"), "abc")    // ["", "abc"]
split(compile("$"), "abc")    // ["abc", ""]
split(compile("^|$"), "abc")  // ["", "abc", ""]
```

`replace_all` and `replace_n` insert `replacement` literally. There is no
`$1`, `\1`, or backslash expansion. `replace_n(..., limit)` replaces at
most `limit` matches; limit 0 returns the original input. Zero-width matches
insert at every boundary, including both ends:

```wz
replace_all(compile("a+"), "baaacaa", "-")   // "b-c-"
replace_all(compile(""), "abc", "-")         // "-a-b-c-"
replace_all(compile("^"), "abc", "-")        // "-abc"
replace_all(compile("$"), "abc", "-")        // "abc-"
replace_n(compile(""), "abc", "-", 2)       // "-a-bc"
```

Anchors in `split`, `replace_all`, and `replace_n` are also tested against the whole input.
For example, `^` can supply at most one separator at index 0.

## Capture semantics

`find_captures` follows the same overall leftmost-longest span as `find`.
Groups are numbered by opening parenthesis order starting at 1, including
nested groups. Group 0 is the overall match; group 1 is the first opening
parenthesis.

The group vector is private. Use the safe accessors instead of indexing it:

```wz
group_count(captures)           // number of capture groups, excluding group 0
group(captures, index)          // Option[Match]
group_text(captures, index)     // Option[string], including group 0
```

`group_text` returns `None` for an out-of-range or non-participating group.
`Captures` retains the source string, so `group_text` remains valid after the
original source binding goes out of scope. Groups in an untaken alternative
are `None`. `group_text(captures, 0)` always describes the overall match.

The tagged runner keeps at most one thread per NFA state and input position.
Thread priority is deterministic: the first alternative and the greedy
repetition branch are visited first. For a fixed overall span, the surviving
highest-priority path determines captures. This is not the POSIX
subexpression-longest rule, and it is not a backtracking engine.

Limit of the bounded model: because a state is not revisited at the same input
position, a final zero-length repetition is not re-entered. A group whose only
remaining participation would be such a final empty iteration keeps its last
non-empty capture (or `None` if it never participated). For example,
`^(a*)*` on `"aaa"` reports group 1 as `[0, 3)` rather than an empty capture at
3, and on `""` group 1 is `None`. Overall matches and all ordinary (non-capture)
APIs are unaffected. `compile` and `compile_captures` both accept at most 32
groups in the capture form. `compile_captures` reports the 33rd opening
parenthesis as `Error.PatternTooLarge`; `compile` falls back to a
non-capturing Pattern as described above.

## Supported syntax and limits

Supported: literals including UTF-8, `.`, `^`, `$`, `(...)`, `|`, `*`, `+`,
`?`, character classes `[a-z]`, negated classes `[^x]`, and the escapes
`\\ \. \* \+ \? \( \) \[ \] \{ \} \| \^ \$ \- \n \r \t`.

Not supported: backreferences, lookaround, `{m,n}`, flags, non-capturing
groups, Unicode property classes, multiline anchors, and capture expansion in
replacement strings. `.` does not match U+000A. `^` and `$` always refer to the
whole input.

Compile limits:

| Limit | Value |
| --- | --- |
| Pattern bytes | 65536 |
| Pattern code points | 4096 |
| Group nesting | 128 |
| NFA states | 16384 |
| Character-class ranges | 16384 |
| Capture groups | 32 |

Compile failures use `Error.Syntax(offset)` for syntax, `Error.PatternTooLarge`
for a size/state/group limit, and `Error.NestingTooDeep(offset)` for nesting.
Offsets are code point indices in the pattern. `CaptureError.Unavailable` is
not a compile failure; it is the separate typed rejection from
`find_captures` and `find_captures_from` when their Pattern has no capture
metadata.

## Performance and pathological input

The ordinary runner visits each NFA state at most once per input code point.
Once a leftmost match exists, it stops adding starts and discards threads with
a later start. It can stop completely when no live thread with the same or an
earlier start can consume another code point. That early stop preserves
earliest-start longest-end semantics: a live longer alternative with the same
start is kept until it dies or matches. `find`, `find_from`, `find_all`,
`split`, `replace_all`, and `replace_n` use the same runner.

`split`, `split_n`, `replace_all`, and `replace_n` create one reusable
runner and convert the input to UTF-8 bytes plus code-point-to-byte offsets
once. Pieces and replacement output are copied from byte ranges, avoiding the
code-point slice runtime's repeated scan from the beginning of the string. A
short separator therefore costs O(segment length * NFA size) per search and
O(n * NFA size) overall for a linear scan of pieces.

Remaining worst case: an ambiguous pattern can keep a same-start thread alive
across a long suffix. For example, `a.*b|a` on a long input with many `a`
characters but no `b` finds the short `a` match immediately, while the
`a.*b` thread remains live to the end. Each split/replace search must then
process that suffix, so repeated separators can still be O(n^2 * NFA size).
Patterns that need a later, longer alternative cannot use the early stop
without changing leftmost-longest results. Some repetition/tag patterns and
pathological capture inputs also remain linear per position but with higher
constants.

The capture runner adds tag vectors to threads. A tag state copies its tag
vector before writing, and one thread per state/position is retained. It uses
the same pruning as the ordinary runner: after a match exists it does not spawn
new starts, discards threads whose start is later than the best match, and
stops when no live thread with the best or an earlier start can consume another
code point. A same-start longer alternative still keeps the scan alive, and the
tagged priority order is unchanged. Its full-scan time is roughly O(input
length * NFA size * capture groups); an early match can stop after the prefix
that determines the leftmost-longest result. It does not change the ordinary
runner.

Each `find_captures` or `find_captures_from` call still converts the complete
input string to code points before matching. Repeating those calls over one
long fixed-size input therefore retains an O(input length) per-call floor even
when the matcher stops early. The benchmark below reports that conversion-only
cost separately, so the matcher pruning is not confused with input decoding.

A fixed-size benchmark used a 4096-code-point `"a"` input, 400 iterations,
`compile_captures`, and a checksum over the returned spans. Input construction
was outside the measured loops. Values are medians of three pinned Linux
x86_64 runs; old is the pre-pruning capture runner:

| Scenario | old O0 | new O0 | old O2 | new O2 |
| --- | ---: | ---: | ---: | ---: |
| `text.runes` only (no matcher) | 43.9 ms | 43.7 ms | 40.5 ms | 33.4 ms |
| `(a)` early match | 6744.7 ms | 58.6 ms | 5239.4 ms | 35.5 ms |
| `()` empty match | 5120.5 ms | 55.8 ms | 5490.2 ms | 34.8 ms |
| `(a)(a)(a)(a)` early match | 18415.9 ms | 71.3 ms | 16389.5 ms | 45.7 ms |
| `(z)` no match (full scan control) | 4212.4 ms | 4320.5 ms | 4796.0 ms | 3718.8 ms |

The early-match results approach the conversion-only floor; the no-match
control remains a full scan in both versions. Checksums matched between old
and new runs. The bounded `check_capture_early_stop` fixture also exercises a
20000-code-point early match and a late match without asserting wall time.

The CTest suite `stdlib.regex_extended` covers short separators,
zero-width matches, capture lifetime, anchors, ambiguous alternatives,
automatic compile captures, explicit `CaptureError` boundaries, capture round
trips, and a bounded capture early-stop case. The ordinary `stdlib_regex`
fixture is unchanged and still exhaustively matches the original `Error`
cases.
